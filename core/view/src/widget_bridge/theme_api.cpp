// widget_bridge/theme_api.cpp - theme registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
// Only the runtime W3C token pair is needed here; use the light always-compiled
// header so the default theme API does not depend on the gated design-import
// cluster (PULP_ENABLE_DESIGN_IMPORT).
#include <pulp/view/w3c_tokens.hpp>
#include "api_registry.hpp"

#include <string>

namespace pulp::view {

void WidgetBridge::register_theme_api() {
    BridgeApiContext api{engine_};

    // Theme control
    register_bridge_function(api, "setTheme", [this](choc::javascript::ArgumentList args) {
        auto n = args.get<std::string>(0, "dark");
        root_.set_theme(n=="light" ? Theme::light() : n=="pro_audio" ? Theme::pro_audio() : Theme::dark());
        return choc::value::Value();
    });

    register_bridge_function(api, "applyTokenDiff", [this](choc::javascript::ArgumentList args) {
        auto json = args.get<std::string>(0, "");
        if (!json.empty()) { auto d = Theme::from_json(json); auto c = root_.theme(); c.apply_overrides(d); root_.set_theme(c); }
        return choc::value::Value();
    });

    register_bridge_function(api, "getThemeJson", [this](choc::javascript::ArgumentList) {
        return choc::value::createString(root_.theme().to_json());
    });

    // importDesignTokens(w3cJson) - parse W3C Design Tokens JSON and apply to theme
    register_bridge_function(api, "importDesignTokens", [this](choc::javascript::ArgumentList args) {
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
    register_bridge_function(api, "exportDesignTokens", [this](choc::javascript::ArgumentList) {
        return choc::value::createString(export_w3c_tokens(root_.theme()));
    });
}

} // namespace pulp::view
