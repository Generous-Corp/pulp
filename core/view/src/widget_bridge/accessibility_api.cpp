// widget_bridge/accessibility_api.cpp - accessibility registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/aria_roles.hpp>
#include "api_registry.hpp"

#include <string>
#include <utility>

namespace pulp::view {

void WidgetBridge::register_accessibility_api() {
    BridgeApiContext api{engine_};

    // setAccessibilityLabel / setAccessibilityRole are the bridge-side
    // entry points the html-compat layer calls when JS does
    //   el.setAttribute('aria-label', '...')
    //   el.setAttribute('role',       '...').
    //
    // Storage lives on View::access_label_ / View::access_role_ (the
    // existing widget-level a11y slots already consumed by the macOS
    // NSAccessibility bridge in core/view/platform/mac/accessibility_mac.mm
    // and the cross-platform AccessibilityTree snapshot in
    // accessibility_tree.cpp). Linux AT-SPI / Windows UIA platform
    // routing remains a platform concern: the bridge entry point is
    // platform-agnostic; JS-side authors can rely on the same surface
    // on every platform and the storage round-trips through getAttribute
    // either way.
    //
    // Role mapping is the shared ARIA table in <pulp/view/aria_roles.hpp>
    // (unit-tested without a JS engine). Unknown / empty role clears the role
    // back to AccessRole::none.
    register_bridge_function(api, "setAccessibilityLabel",
                             [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto label = args.get<std::string>(1, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end()) it->second->set_access_label(std::move(label));
        return choc::value::Value();
    });

    register_bridge_function(api, "setAccessibilityRole",
                             [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto role = args.get<std::string>(1, "");
        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::Value();

        // ARIA role token -> View::AccessRole. Single shared table so the
        // JS bridge, the widget defaults, and the platform mappings agree.
        const View::AccessRole r = access_role_from_aria(role);
        it->second->set_access_role(r);
        return choc::value::Value();
    });

    // ARIA state attributes (aria-pressed / aria-checked / aria-disabled /
    // aria-hidden). Tri-state per ARIA 1.2: `true` /
    // `false` / `mixed` / unset. We store the raw string the JS shim
    // hands us so platform AT bridges can read it back verbatim.
    // Single bridge fn rather than four; `attr` selects the slot.
    register_bridge_function(api, "setAccessibilityState",
                             [this](choc::javascript::ArgumentList args) {
        auto id    = args.get<std::string>(0, "");
        auto attr  = args.get<std::string>(1, "");
        auto value = args.get<std::string>(2, "");
        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::Value();
        if      (attr == "pressed")  it->second->set_access_pressed(std::move(value));
        else if (attr == "checked")  it->second->set_access_checked(std::move(value));
        else if (attr == "disabled") it->second->set_access_disabled(std::move(value));
        else if (attr == "hidden")   it->second->set_access_hidden(std::move(value));
        // Unknown aria-* state attr is a no-op for forward compatibility.
        return choc::value::Value();
    });
}

} // namespace pulp::view
