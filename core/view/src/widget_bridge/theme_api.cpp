// widget_bridge/theme_api.cpp - theme registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
// Only the runtime W3C token pair is needed here; use the light always-compiled
// header so the default theme API does not depend on the gated design-import
// cluster (PULP_ENABLE_DESIGN_IMPORT).
#include <pulp/view/w3c_tokens.hpp>
#include "api_registry.hpp"

#include <string>

namespace pulp::view {

void BridgeRegistrars::register_theme_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // Theme control
    register_bridge_function(api, "setTheme", [&self](choc::javascript::ArgumentList args) {
        auto n = args.get<std::string>(0, "dark");
        self.root_.set_theme(n=="light" ? Theme::light() : n=="pro_audio" ? Theme::pro_audio() : Theme::dark());
        return choc::value::Value();
    });

    register_bridge_function(api, "applyTokenDiff", [&self](choc::javascript::ArgumentList args) {
        auto json = args.get<std::string>(0, "");
        if (!json.empty()) { auto d = Theme::from_json(json); auto c = self.root_.theme(); c.apply_overrides(d); self.root_.set_theme(c); }
        return choc::value::Value();
    });

    register_bridge_function(api, "getThemeJson", [&self](choc::javascript::ArgumentList) {
        return choc::value::createString(self.root_.theme().to_json());
    });

    // importDesignTokens(w3cJson) - parse W3C Design Tokens JSON and apply to theme
    register_bridge_function(api, "importDesignTokens", [&self](choc::javascript::ArgumentList args) {
        auto json = args.get<std::string>(0, "");
        if (!json.empty()) {
            auto imported = parse_w3c_tokens(json);
            auto current = self.root_.theme();
            current.apply_overrides(imported);
            self.root_.set_theme(current);
        }
        return choc::value::Value();
    });

    // exportDesignTokens() - export current theme as W3C Design Tokens JSON
    register_bridge_function(api, "exportDesignTokens", [&self](choc::javascript::ArgumentList) {
        return choc::value::createString(export_w3c_tokens(self.root_.theme()));
    });
}

} // namespace pulp::view
