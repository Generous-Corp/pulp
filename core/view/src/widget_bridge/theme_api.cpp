// widget_bridge/theme_api.cpp - theme registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/design_tokens.hpp>

#include <string>

namespace pulp::view {

void WidgetBridge::register_theme_api() {
    // Theme control
    engine_.register_function("setTheme", [this](choc::javascript::ArgumentList args) {
        auto n = args.get<std::string>(0, "dark");
        root_.set_theme(n=="light" ? Theme::light() : n=="pro_audio" ? Theme::pro_audio() : Theme::dark());
        return choc::value::Value();
    });

    engine_.register_function("applyTokenDiff", [this](choc::javascript::ArgumentList args) {
        auto json = args.get<std::string>(0, "");
        if (!json.empty()) { auto d = Theme::from_json(json); auto c = root_.theme(); c.apply_overrides(d); root_.set_theme(c); }
        return choc::value::Value();
    });

    engine_.register_function("getThemeJson", [this](choc::javascript::ArgumentList) {
        return choc::value::createString(root_.theme().to_json());
    });

    // importDesignTokens(w3cJson) - parse W3C Design Tokens JSON and apply to theme
    engine_.register_function("importDesignTokens", [this](choc::javascript::ArgumentList args) {
        auto json = args.get<std::string>(0, "");
        if (!json.empty()) {
            auto imported = parse_w3c_tokens(json);
            auto current = root_.theme();
            current.apply_overrides(imported);
            root_.set_theme(current);
        }
        return choc::value::Value();
    });

    // exportDesignTokens() - export current theme as W3C Design Tokens JSON
    engine_.register_function("exportDesignTokens", [this](choc::javascript::ArgumentList) {
        return choc::value::createString(export_w3c_tokens(root_.theme()));
    });
}

} // namespace pulp::view
