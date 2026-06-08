// widget_bridge/tokens_api.cpp - theme-token registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>

#include <functional>
#include <string>
#include <utility>

namespace pulp::view {

void WidgetBridge::register_tokens_api(std::function<canvas::Color(const std::string&)> parse_color) {
    auto parseColor = std::move(parse_color);

    // setMotionToken(tokenName, value)
    engine_.register_function("setMotionToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto value = static_cast<float>(args.get<double>(1, 0));
        if (name.empty()) return choc::value::Value();
        auto theme = root_.theme();
        theme.dimensions[name] = value;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // getMotionToken(tokenName) -> value
    engine_.register_function("getMotionToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto d = root_.theme().dimension(name);
        return choc::value::createFloat64(d.value_or(0.0f));
    });

    // pulp #1899 (gap #3) - string-valued theme-token lookup.
    // setStringToken(name, value) / getStringToken(name) parallel the
    // motion-token / color-token APIs but back onto `theme.strings`
    // (the same map that already stores design-system font names
    // imported via Stitch / W3C tokens - see design_import.cpp). The
    // React-side prop-applier consults this to resolve `var(--mono)`
    // in `fontFamily` / `color` / `borderColor` etc. before forwarding
    // to setFontFamily / setTextColor - without this, Skia's font
    // matcher received the literal string "var(--mono)" as a family
    // name and fell through to a proportional sans, which is visible
    // as the Spectr top-bar "faint label" symptom from #1899.
    engine_.register_function("setStringToken", [this](choc::javascript::ArgumentList args) {
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
    engine_.register_function("getStringToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto s = root_.theme().string_token(name);
        return choc::value::createString(s.value_or(std::string()));
    });

    // setColorToken(name, color) - set a color token on the root theme
    engine_.register_function("setColorToken", [this, parseColor](choc::javascript::ArgumentList args) {
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
    engine_.register_function("setDimensionToken", [this](choc::javascript::ArgumentList args) {
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
